namespace SampleApp.Models
{
    public class User
    {
        public string Id { get; set; }
        public string Name { get; set; }

        public bool IsAnonymous()
        {
            return string.IsNullOrEmpty(Name);
        }
    }

    public enum UserRole
    {
        Guest,
        Member,
        Admin,
    }

    public interface IUserRepository
    {
        User FindById(string id);
        void Save(User user);
    }
}
